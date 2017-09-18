<?php

/**
 * @file
 * Provides TMGMT DGT Connector translation plugin controller.
 */

use EC\Poetry\Poetry;
use EC\Poetry\Messages\Components;
use EC\Poetry\Exceptions;

/**
 * TMGMT DGT Connector translation plugin controller.
 *
 * It's now mostly based on TMGMTPoetryTranslatorPluginController
 * Later this should extend TMGMTDefaultTranslatorPluginController and
 * TMGMTPoetryTranslatorPluginController should be deprecated.
 */
class TmgmtDgtConnectorTranslatorPluginController extends TMGMTPoetryTranslatorPluginController {

  /**
   * Implements TMGMTTranslatorPluginControllerInterface::canTranslation().
   *
   * Check if source and target languages are supported.
   */
  public function canTranslate(TMGMTTranslator $translator, TMGMTJob $job) {
    // Check that the item does not have pending requests.
    $items = $job->getItems();
    $item = array_shift($items);
    if (!empty($item)) {
      $results = db_select('tmgmt_job_item', 'tji')
        ->fields('tji', array('tjid', 'tjiid'))
        ->condition('item_type', $item->item_type, '=')
        ->condition('item_id', $item->item_id, '=')
        ->execute()
        ->fetchAllAssoc('tjid');
      if ($results) {
        foreach ($results as $result) {
          if ($job = tmgmt_job_load($result->tjid)) {
            if ($job->hasPoetryTranslator() && $job->getState() == 1) {
              // Check the poetry status.
              $result_ps = db_select('poetry_status', 'ps')
                ->fields('ps', array('status'))
                ->condition('tjiid', $result->tjiid, '=')
                ->execute()
                ->fetchAssoc();
              if (empty($result_ps) || (!empty($result_ps) && $result_ps['status'] == POETRY_STATUS_MESSAGE_PEN)) {
                return FALSE;
              }
            }
          }
        }
      }
    }
    // All languages are supported at the moment.
    return TRUE;
  }

  /**
   * {@inheritdoc}
   */
  public function getNotCanTranslateReason(TMGMTJob $job) {
    return t('A pending translation for this item already exists');
  }

  /**
   * {@inheritdoc}
   */
  public function requestTranslation(TMGMTJob $job) {

    // Request translation from Poetry.
    $response = $this->sendRequest($job);

    // Handling response.
    $response_xml = simplexml_load_string($response);

    // If it is not an xml response, the SOAP request failed.
    if (!$response_xml) {
      _tmgmt_poetry_tmgmt_job_reject(
        $job,
        "Rejected",
        "There was an error with the SOAP service.",
        $response
      );
    }
    else {
      $request = $response_xml->request;

      $statuses = $request->status;

      // Check the statuses of the response
      // in order to verify that everything is correct.
      foreach ($statuses as $status) {
        $message = (string) $status->statusMessage;
        if ($message != 'OK') {
          _tmgmt_poetry_tmgmt_job_reject(
            $job,
            "Rejected",
            "There was an error with the Poetry request.",
            $message
          );
        }
      }

      if (isset($request->demandeId)) {
        // Insert or update the mapping information.
        $demande_id = (array) $request->demandeId;
        $items = $job->getItems();
        $item = array_shift($items);

        $result = db_select('poetry_map', 'pm')
          ->fields('pm', array('annee', 'numero', 'version', 'partie'))
          ->condition('entity_type', $item->item_type, '=')
          ->condition('entity_id', $item->item_id, '=')
          ->execute()
          ->fetchAssoc();
        if (!$result) {
          db_insert('poetry_map')
            ->fields(array(
              'entity_type' => $item->item_type,
              'entity_id' => $item->item_id,
              'annee' => $demande_id['annee'],
              'numero' => $demande_id['numero'],
              'version' => $demande_id['version'],
              'partie' => $demande_id['partie'],
            ))
            ->execute();
        }
        else {
          // @FIXME Do not overwrite data. Might not be for the same revision.
          db_update('poetry_map')
            ->fields(array(
              'annee' => $demande_id['annee'],
              'numero' => $demande_id['numero'],
              'version' => $demande_id['version'],
              'partie' => $demande_id['partie'],
            ))
            ->condition('entity_type', $item->item_type, '=')
            ->condition('entity_id', $item->item_id, '=')
            ->execute();
        }
      }

      if (!$job->isRejected()) {
        if (strpos($job->reference, '_POETRY_') === FALSE) {
          $job->reference .= '_POETRY_' . implode('/', $demande_id);
        }
        $job->submitted('Job has been successfully submitted for translation. Project ID is: %project_id',
          array('%project_id' => $job->reference));
      }
    }
  }

  /**
   * Execute a request against the Poetry API.
   */
  protected function sendRequest(TMGMTJob $job) {
    global $base_url;

    // Generate the identifier for the request.
    $translator = $job->getTranslator();
    $items = $job->getItems();
    $item = array_shift($items);
    $settings = $translator->getSetting('settings');
    $id_data = $this->generateRequestId($item, $settings);

    // Get other pieces of information.
    $organization = $job->settings['organization'];
    $details_title = 'NE-CMS: ';
    $details_title .= (empty($settings['website_identifier']) ? '' : $settings['website_identifier'] . ' - ');
    $details_title .= $job->label;

    // Generate the callback parameters for the request.
    $callback_address = url(drupal_get_path("module", "tmgmt_poetry") . "/wsdl/PoetryIntegration.wsdl", array(
      'absolute' => TRUE,
      'language' => (object) array('language' => FALSE),
    ));

    // Generate the data into a XML format and encode it to be translated.
    $controller = tmgmt_file_format_controller($job->getSetting('export_format'));
    $export = $controller->export($job);
    $content = base64_encode($export);

    try {
      $poetry = new Poetry([
        'identifier.code' => $id_data['codeDemandeur'],
        'identifier.year' => $id_data['annee'],
        'identifier.number' => $id_data['sequence'],
        'identifier.version' => $id_data['version'],
        'identifier.part' => $id_data['partie'],
        'identifier.product' => $id_data['produit'],
        'authentication.username' => $settings['poetry_user'],
        'authentication.password' => $settings['poetry_password'],
        'server.uri' => $callback_address,
      ]);

      // Start building the request message.
      $message = $poetry->get('message.request');

      $details = (new Components\Details())
        ->setClientId('Job ID ' . $job->tjid)
        ->setTitle($details_title)
        ->setRequester($organization['demandeur'])
        ->setAuthor($organization['auteur'])
        ->setApplicationId('FPFIS')
        ->setDelay(date('d/m/Y', strtotime($job->settings['delai'])))
        ->setRemark('')
        ->setReferenceFilesRemark($base_url)
        ->setProcedure('NEANT')
        ->setDestination('PUBLIC')
        ->setType('INTER');
      $message->setDetails($details);

      $contacts = [];
      $contacts['secretaire'] = (new Components\Contact())
        ->setType('Secretaire')
        ->setNickname('ecusername');
      $contacts['contact'] = (new Components\Contact())
        ->setType('Contact')
        ->setNickname('ecusername');
      $contacts['responsable'] = (new Components\Contact())
        ->setType('Responsible')
        ->setNickname('ecusername');
      $message->setContacts($contacts);

      $retour = (new Components\ReturnAddress())
        ->setType('webService')
        ->setAction('UPDATE')
        ->setUser('MockCallback')
        ->setPassword('MockCallbackPWD')
        ->setAddress($callback_address)
        // Missing ->setPath('FPFISPoetryIntegrationRequest') .
        ->setRemark('');
      $message->setReturnAddress($retour);

      $document_source = (new Components\ReferenceDocument())
        ->setType('REF')
        ->setFormat('HTML')
        ->setName('content.html')
        ->setLanguage('EN')
        ->setFile($content);
      $message->setReferenceDocuments([$document_source]);

      // To test do
      // $renderer = $poetry->get('renderer');
      // dpm($renderer->render($message));
      // .
      $response = $poetry->getClient()->send($message);

      return $response;

    }
    catch (Exceptions\ValidationException $exception) {
      drupal_set_message($exception->getMessage(), 'error');
      return FALSE;
    }

  }

  /**
   * Helper method to generate a request id.
   *
   * @param TMGMTJobItem $item
   *   Item to search for.
   * @param array $settings
   *   Settings.
   */
  protected function generateRequestId(TMGMTJobItem $item, array $settings) {

    // Always request a new numero.
    $id_data = array(
      'codeDemandeur' => $settings['code'],
      'annee' => date("Y"),
      'sequence' => $settings['counter'],
      'version' => 00,
      'partie' => 00,
      'produit' => 'TRA',

    );
    return $id_data;
  }

}